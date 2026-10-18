// o11proto - Live UDP, the client side: hellos, the cookie and the drop
// rules of PROTOCOL.md 15.5-15.6, and the direct paths of 15.10.
#include "o11proto/live_transport.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "o11proto/connection.h"
#include "o11proto/realtime.h"
#include "socket_compat.h"
#include "udp_compat.h"

namespace o11proto {
namespace live {

namespace {

bool isLoopback(std::uint32_t ip_net) { return (ntohl(ip_net) >> 24) == 127; }

}  // namespace

// One peer's direct path (15.10): where it can be reached (its LAN
// address, its public one, and one its datagrams came from), and the
// state of the pair.
struct DirectPath {
    enum State { Opening, Up, Suspect, Down };
    static constexpr int kLan = 0, kPublic = 1, kSeen = 2, kCands = 3;
    std::uint32_t id = 0;
    std::uint64_t key = 0;
    udp::Addr cand[kCands];                    // ip 0: none
    TimePoint replied[kCands] = {};            // the last answer to a probe sent there
    State state = Opening;
    TimePoint opened{}, last_probe{}, last_reply{};
    int active = -1;                           // the address it sends to while up
    double rtt_ms = -1;
    // The test hooks: blocked, nothing goes either way on the wire (the
    // probe rule finds it, as it would a network's); forced, no direct path
    // at all (unreachable at once, never probed).
    bool blocked = false, forced = false;
    bool anywhere() const {
        for (const auto& c : cand)
            if (c.ip != 0) return true;
        return false;
    }
    int find(const udp::Addr& a) const {
        for (int k = 0; k < kCands; ++k)
            if (cand[k].ip != 0 && cand[k].ip == a.ip && cand[k].port == a.port) return k;
        return -1;
    }
    bool direct() const { return !forced && (state == Up || state == Suspect) && active >= 0; }
    PathState reported() const {
        return state == Up || state == Suspect ? PathState::Up : state == Down ? PathState::Down : PathState::Opening;
    }
    // The address to send to: the LAN's while it answers, else the public
    // one's, else the one it was seen at.
    void choose(TimePoint now) {
        active = -1;
        for (int k = 0; k < kCands && active < 0; ++k)
            if (cand[k].ip != 0 && replied[k] != TimePoint{} &&
                now - replied[k] <= std::chrono::milliseconds(kPathDeadMs))
                active = k;
    }
    void open(TimePoint now) {                 // probed afresh, through the relay meanwhile
        state = Opening;
        opened = now;
        last_probe = TimePoint{};
        active = -1;
        for (auto& r : replied) r = TimePoint{};
    }
};

struct Transport::Impl {
    Options opt;
    socket_t fd = kInvalidSocket;
    udp::Addr relay;                           // where the hellos go and the relay's datagrams come from
    std::uint32_t lan_ip = 0;                  // localAddress()
    std::uint16_t local_port = 0;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::uint8_t buf[2048];
    std::uint64_t stamps[kHelloTimestamps] = {};   // the last hellos' timestamps
    int next_stamp = 0;
    TimePoint last_hello;
    std::atomic<double> loss{0};                   // the test hook
    std::uint32_t rng = 0x9e3779b9u;              // and its generator
    std::atomic<std::int64_t> gap_until_ns{0};     // the send gap's end (steady clock)
    std::atomic<std::int64_t> hold_until_ns{0};    // the send hold's end
    struct Held {
        udp::Addr to;
        bool direct = false;
        std::vector<std::uint8_t> bytes;
    };
    std::mutex held_mutex;                         // what it holds: send() and stop()
    std::vector<Held> held;
    // The direct paths: set from any thread, used by the network thread,
    // read by the accessors. The network thread holds the lock only
    // briefly and never while it calls back.
    mutable std::mutex paths_mutex;
    std::vector<DirectPath> paths;
    bool relay_fallback = true;                    // setRoomRoute(): until a room message says
    bool room_relay = false;
    bool force_relay = false;
    std::vector<std::uint32_t> blocked;
    // wake(): a non-blocking pipe, open for the transport's life (the audio
    // thread may write while the network thread stops), and a flag so that
    // it is written once until the network thread has woken.
    int wake_fd[2] = {-1, -1};
    std::atomic<bool> wake_pending{false};
    std::atomic<bool> latency_critical{false}, realtime{false};
    std::atomic<std::uint64_t> timeouts{0}, over_5ms{0}, wakes{0}, wake_over_5ms{0};
    std::atomic<std::int64_t> longest_us{0}, wake_longest_us{0};
    std::atomic<std::int64_t> woken_at_us{0};      // wake(): when it wrote to the pipe
    int wait_ms = 1;
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
    DirectPath* path(std::uint32_t id) {           // paths_mutex held
        for (auto& p : paths)
            if (p.id == id) return &p;
        return nullptr;
    }
    const DirectPath* path(std::uint32_t id) const {
        for (const auto& p : paths)
            if (p.id == id) return &p;
        return nullptr;
    }
    void applyBlocks(TimePoint now) {              // paths_mutex held
        for (auto& p : paths) {
            p.blocked = std::find(blocked.begin(), blocked.end(), p.id) != blocked.end();
            if (force_relay && !p.forced) {
                p.state = DirectPath::Down;
                p.active = -1;
            } else if (!force_relay && p.forced) {
                p.open(now);                       // allowed again: it opens at once
            }
            p.forced = force_relay;
        }
    }
    // A peer's address as 0x09 gives it: a loopback one is the relay's own
    // machine, so the relay's address, unless the relay is on loopback too.
    udp::Addr peerAddr(std::uint32_t ip, std::uint16_t port, bool lan) const {
        udp::Addr a;
        if (ip == 0 || port == 0 || udp::isBroadcastOrMulticast(ip)) return a;
        if (isLoopback(ip) && !isLoopback(relay.ip)) {
            if (lan) return a;                     // its LAN can't be loopback
            ip = relay.ip;
        }
        a.ip = ip;
        a.port = htons(port);
        if (a.ip == relay.ip && a.port == relay.port) return udp::Addr();
        return a;
    }
};

namespace {

std::uint64_t monotonicMicros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

// The client header (20 bytes) of a datagram from `sender`.
void putHeader(std::uint8_t* d, std::uint8_t type, std::uint32_t sender, std::uint64_t key, std::uint32_t dest) {
    d[0] = kMagic0;
    d[1] = kMagic1;
    d[2] = kUdpVersion;
    d[3] = type;
    putLE32(d + 4, sender);
    putLE64(d + 8, key);
    putLE32(d + 16, dest);
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
    m.woken_at_us.store(static_cast<std::int64_t>(monotonicMicros()));
    const char c = 1;
    if (::write(m.wake_fd[1], &c, 1) != 1) m.wake_pending.store(false);
#endif
}

Transport::Timing Transport::timing() const {
    const Impl& m = *impl_;
    Timing t;
    t.latency_critical = m.latency_critical.load();
    t.realtime = m.realtime.load();
    t.wait_ms = m.wait_ms;
    t.timeouts = m.timeouts.load();
    t.over_5ms = m.over_5ms.load();
    t.wakes = m.wakes.load();
    t.wake_over_5ms = m.wake_over_5ms.load();
    t.longest_ms = static_cast<double>(m.longest_us.load()) / 1000.0;
    t.wake_longest_ms = static_cast<double>(m.wake_longest_us.load()) / 1000.0;
    return t;
}

bool Transport::running() const { return impl_->running.load(); }
void Transport::setTestReceiveLoss(double fraction) { impl_->loss.store(fraction); }
void Transport::setTestSendGap(int ms) {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    impl_->gap_until_ns.store(now + static_cast<std::int64_t>(ms) * 1000000);
}
void Transport::setTestSendHold(int ms) {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    impl_->hold_until_ns.store(now + static_cast<std::int64_t>(ms) * 1000000);
}

// ------------------------------------------------------------ the paths

void Transport::setPaths(const std::vector<PathPeer>& peers) {
    Impl& m = *impl_;
    const TimePoint now = Clock::now();
    std::lock_guard<std::mutex> lock(m.paths_mutex);
    std::vector<DirectPath> next;
    for (const PathPeer& pp : peers) {
        if (pp.id == 0 || pp.id == m.opt.peer_id || pp.key == 0) continue;
        bool dup = false;
        for (const auto& q : next) dup |= q.id == pp.id;
        if (dup) continue;
        const DirectPath* old = m.path(pp.id);
        DirectPath p = old ? *old : DirectPath();
        p.id = pp.id;
        const udp::Addr lan = m.peerAddr(pp.lan_ip, pp.lan_port, true);
        udp::Addr pub = m.peerAddr(pp.public_ip, pp.public_port, false);
        if (pub.ip == lan.ip && pub.port == lan.port) pub = udp::Addr();   // the same address once
        const bool had = p.anywhere();
        const bool moved = p.cand[DirectPath::kLan].ip != lan.ip || p.cand[DirectPath::kLan].port != lan.port ||
                           p.cand[DirectPath::kPublic].ip != pub.ip ||
                           p.cand[DirectPath::kPublic].port != pub.port;
        const int was_active = p.active;
        p.cand[DirectPath::kLan] = lan;
        p.cand[DirectPath::kPublic] = pub;
        if (!old || p.key != pp.key) {         // a new pair: it opens from the start
            p.key = pp.key;
            p.cand[DirectPath::kSeen] = udp::Addr();
            p.rtt_ms = -1;
            p.open(now);
        } else if (!had && p.anywhere()) {     // its first address: the opening starts now
            p.open(now);
        } else if (moved) {
            if (was_active == DirectPath::kLan || was_active == DirectPath::kPublic) {
                p.replied[was_active] = TimePoint{};
                if (p.state == DirectPath::Up) p.state = DirectPath::Suspect;   // probed again at once
                p.last_probe = TimePoint{};
            }
        }
        next.push_back(p);
    }
    m.paths.swap(next);
    m.applyBlocks(now);
}

bool Transport::hasPaths() const {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    return !impl_->paths.empty();
}

Transport::Route Transport::route(std::uint32_t peer) const {
    if (!running()) return Route::None;
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    const Impl& m = *impl_;
    const DirectPath* p = m.path(peer);
    if (m.room_relay || m.force_relay || !p) return Route::Relay;
    return p->direct() ? Route::Direct : m.relay_fallback ? Route::Relay : Route::Off;
}

bool Transport::pathUp(std::uint32_t peer) const {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    const DirectPath* p = impl_->path(peer);
    return p && p->direct();
}

void Transport::setRoomRoute(bool relay_fallback, bool room_relay) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    impl_->relay_fallback = relay_fallback;
    impl_->room_relay = room_relay;
}

std::vector<PathsReport::Entry> Transport::pathStates() const {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    std::vector<PathsReport::Entry> v;
    for (const DirectPath& p : impl_->paths) {
        PathsReport::Entry e;
        e.id = p.id;
        e.state = p.reported();
        v.push_back(e);
    }
    return v;
}

double Transport::probeRttMs(std::uint32_t peer) const {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    const DirectPath* p = impl_->path(peer);
    return p ? p->rtt_ms : -1;
}

void Transport::localAddress(std::uint32_t* lan_ip, std::uint16_t* port) const {
    if (lan_ip) *lan_ip = impl_->lan_ip;
    if (port) *port = impl_->local_port;
}

void Transport::setTestForceRelay(bool on) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    impl_->force_relay = on;
    impl_->applyBlocks(Clock::now());
}

void Transport::setTestBlocked(const std::vector<std::uint32_t>& peers) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex);
    impl_->blocked = peers;
    impl_->applyBlocks(Clock::now());
}

// ------------------------------------------------------- start and stop

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
    // One socket for the relay and the direct paths, on a port of its own.
    // It isn't connect()ed: what comes from the relay's address and port is
    // the relay's, anything else must be a peer's (with its pair's key).
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0) {
        error = "can't open a UDP socket (error " + std::to_string(compat::lastError()) + ")";
        compat::closeSocket(fd);
        return false;
    }
    {
        sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        m.local_port = ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) == 0 ? ntohs(sa.sin_port) : 0;
    }
    // The LAN address: the one the system sends to the relay from (a
    // connect()ed socket of its own sends nothing). Loopback is none.
    m.lan_ip = 0;
    if (socket_t probe = ::socket(AF_INET, SOCK_DGRAM, 0); probe != kInvalidSocket) {
        sockaddr_in sa = udp::toSockaddr(to);
        if (::connect(probe, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0) {
            sockaddr_in me{};
            socklen_t len = sizeof(me);
            if (::getsockname(probe, reinterpret_cast<sockaddr*>(&me), &len) == 0 &&
                !isLoopback(me.sin_addr.s_addr) && me.sin_addr.s_addr != 0)
                m.lan_ip = me.sin_addr.s_addr;
        }
        compat::closeSocket(probe);
    }
    m.fd = fd;
    m.relay = to;
    m.stopping.store(false);
    m.drainWake();
    m.wake_pending.store(false);
    m.timeouts.store(0);
    m.over_5ms.store(0);
    m.wakes.store(0);
    m.wake_over_5ms.store(0);
    m.longest_us.store(0);
    m.wake_longest_us.store(0);
#ifdef _WIN32
    m.wait_ms = 1;                             // wake() does nothing here: the passes are the only tick
#else
    m.wait_ms = m.wake_fd[0] >= 0 ? std::max(1, std::min(100, o.wait_ms)) : 1;
#endif
    for (auto& s : m.stamps) s = 0;
    m.next_stamp = 0;
    m.rng = 0x9e3779b9u ^ o.peer_id;
    m.loss.store(o.test_receive_loss);
    {
        // A new socket and a new id: no path is known yet (setPaths() again).
        std::lock_guard<std::mutex> lock(m.paths_mutex);
        m.paths.clear();
    }
    answered_.store(false);
    rtt_ms_.store(-1.0);
    cookie_.store(0);
    sent_.store(0);
    received_.store(0);
    dropped_.store(0);
    direct_sent_.store(0);
    direct_received_.store(0);
    sendHello();                               // the first one at once
    m.running.store(true);
    m.thread = std::thread([this] { loop(); });
    return true;
}

void Transport::stop() {
    Impl& m = *impl_;
    if (!m.running.load()) return;
    m.stopping.store(true);
    if (m.thread.joinable()) m.thread.join();
    compat::closeSocket(m.fd);
    m.fd = kInvalidSocket;
    {
        // What the test's hold kept goes with the socket: it carries this
        // session's peer ID and keys.
        std::lock_guard<std::mutex> lock(m.held_mutex);
        m.held.clear();
    }
    m.running.store(false);
}

// ------------------------------------------------------------- sending

bool Transport::send(std::uint32_t dest, const std::uint8_t* payload, std::size_t len) {
    Impl& m = *impl_;
    if (m.fd == kInvalidSocket || len == 0 || len > kMaxPayload) return false;
    // Where it goes: directly (with the pair's key) while the path is up,
    // through the relay while it isn't, and both ways while it is suspect.
    udp::Addr direct_to;
    std::uint64_t pair_key = 0;
    bool direct = false, relayed = true, lost = false;
    if (dest != 0) {
        std::lock_guard<std::mutex> lock(m.paths_mutex);
        const DirectPath* p = m.path(dest);
        if (!m.room_relay && !m.force_relay && p) {
            if (p->direct()) {
                direct = true;
                direct_to = p->cand[p->active];
                pair_key = p->key;
                relayed = m.relay_fallback && p->state == DirectPath::Suspect;
                lost = p->blocked;             // the test's block: sent into nothing
            } else if (!m.relay_fallback) {
                return true;                   // direct only: nothing goes to it while its path isn't up
            }
        }
    }
    std::uint8_t d[kClientHeader + kMaxPayload];
    std::memcpy(d + kClientHeader, payload, len);
    const std::size_t n = kClientHeader + len;
    std::uint8_t r[kClientHeader + kMaxPayload];
    if (direct && relayed) std::memcpy(r + kClientHeader, payload, len);
    if (direct) putHeader(d, kTypeData, m.opt.peer_id, pair_key, dest);
    std::uint8_t* rd = direct ? r : d;         // the relay's copy
    if (relayed) putHeader(rd, kTypeData, m.opt.peer_id, m.opt.key, dest);
    const std::int64_t gap = m.gap_until_ns.load(std::memory_order_relaxed);
    const std::int64_t hold = m.hold_until_ns.load(std::memory_order_relaxed);
    if (gap != 0 || hold != 0) {
        const std::int64_t now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        if (now < gap) return true;            // the test's gap
        if (hold != 0) {                       // the test's hold
            std::lock_guard<std::mutex> lock(m.held_mutex);
            if (now < hold) {
                if (m.held.size() < 1024) {
                    if (direct && !lost) m.held.push_back({direct_to, true, std::vector<std::uint8_t>(d, d + n)});
                    if (relayed) m.held.push_back({m.relay, false, std::vector<std::uint8_t>(rd, rd + n)});
                }
                return true;
            }
            for (const auto& h : m.held)
                if (udp::sendTo(m.fd, h.bytes.data(), h.bytes.size(), h.to, 0, false) == 0) {
                    sent_.fetch_add(1);
                    if (h.direct) direct_sent_.fetch_add(1);
                }
            m.held.clear();
        }
    }
    bool ok = lost;
    if (direct && !lost && udp::sendTo(m.fd, d, n, direct_to, 0, false) == 0) {
        sent_.fetch_add(1);
        direct_sent_.fetch_add(1);
        ok = true;
    }
    if (relayed && udp::sendTo(m.fd, rd, n, m.relay, 0, false) == 0) {
        sent_.fetch_add(1);
        ok = true;
    }
    return ok;
}

void Transport::sendHello() {
    Impl& m = *impl_;
    std::uint8_t h[kHelloSize] = {};
    putHeader(h, kTypeHello, m.opt.peer_id, m.opt.key, 0);
    const std::uint64_t stamp = monotonicMicros() | (1ULL << 63);   // never 0
    putLE64(h + 20, stamp);
    putLE64(h + 28, cookie_.load());
    m.stamps[m.next_stamp] = stamp;
    m.next_stamp = (m.next_stamp + 1) % kHelloTimestamps;
    m.last_hello = Clock::now();
    if (udp::sendTo(m.fd, h, sizeof(h), m.relay, 0, false) == 0) sent_.fetch_add(1);
}

// The probes of each path (15.10), from the network thread's passes:
// every kProbeFastMs while it opens and while it is suspect, else every
// kProbeIntervalMs; to the address it sends to while up, to each of its
// addresses (the LAN's first) otherwise.
void Transport::probePaths() {
    Impl& m = *impl_;
    const TimePoint now = Clock::now();
    struct Probe {
        udp::Addr to;
        std::uint8_t d[kProbeSize];
    };
    Probe out[16];
    int n_out = 0;
    {
        std::lock_guard<std::mutex> lock(m.paths_mutex);
        for (DirectPath& p : m.paths) {
            if (p.forced || !p.anywhere()) continue;
            const auto since_reply = now - p.last_reply;
            if (p.state == DirectPath::Up && since_reply > std::chrono::milliseconds(kPathSuspectMs))
                p.state = DirectPath::Suspect;
            if (p.state == DirectPath::Suspect && since_reply > std::chrono::milliseconds(kPathDeadMs)) {
                p.state = DirectPath::Down;    // unreachable: through the relay, probed now and then
                p.active = -1;
            }
            if (p.state == DirectPath::Opening && now - p.opened > std::chrono::milliseconds(kPathDeadMs))
                p.state = DirectPath::Down;
            const bool fast = p.state == DirectPath::Suspect ||
                              (p.state == DirectPath::Opening && now - p.opened < std::chrono::milliseconds(kPathOpenMs));
            if (now - p.last_probe < std::chrono::milliseconds(fast ? kProbeFastMs : kProbeIntervalMs)) continue;
            p.last_probe = now;
            if (p.blocked) continue;           // the test's block: nothing reaches the peer
            const std::uint64_t stamp = monotonicMicros() | (1ULL << 63);
            for (int k = 0; k < DirectPath::kCands && n_out < 16; ++k) {
                if (p.cand[k].ip == 0 || (p.state == DirectPath::Up && k != p.active)) continue;
                Probe& pr = out[n_out++];
                pr.to = p.cand[k];
                std::memset(pr.d, 0, sizeof(pr.d));
                putHeader(pr.d, kTypeProbe, m.opt.peer_id, p.key, p.id);
                putLE64(pr.d + 20, stamp);
                pr.d[28] = static_cast<std::uint8_t>(k);
            }
        }
    }
    for (int i = 0; i < n_out; ++i)
        if (udp::sendTo(m.fd, out[i].d, sizeof(out[i].d), out[i].to, 0, false) == 0) sent_.fetch_add(1);
}

// ------------------------------------------------------------ receiving

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
    if (on_data) on_data(origin, d + kServerHeader, n - kServerHeader, false);
}

// A datagram from anywhere but the relay: a peer's, directly, when it
// carries the pair's key and this client's id (15.10). Probes are
// answered to where they came from; an answer to one of ours brings the
// path up; data goes to on_data.
void Transport::handleDirect(const std::uint8_t* d, std::size_t n, std::uint32_t ip, std::uint16_t port) {
    Impl& m = *impl_;
    if (n < kClientHeader || d[0] != kMagic0 || d[1] != kMagic1 || d[2] != kUdpVersion ||
        !((d[3] == kTypeData && n > kClientHeader && n - kClientHeader <= kMaxPayload) ||
          ((d[3] == kTypeProbe || d[3] == kTypeProbeReply) && n == kProbeSize))) {
        dropped_.fetch_add(1);
        return;
    }
    const std::uint32_t sender = getLE32(d + 4);
    const std::uint64_t key = getLE64(d + 8);
    const std::uint32_t dest = getLE32(d + 16);
    udp::Addr from;
    from.ip = ip;
    from.port = port;
    const TimePoint now = Clock::now();
    std::uint8_t reply[kProbeSize];
    bool answer = false;
    {
        std::lock_guard<std::mutex> lock(m.paths_mutex);
        DirectPath* p = m.path(sender);
        if (!p || p->blocked || p->forced || dest != m.opt.peer_id || !constantTimeEqual(key, p->key)) {
            dropped_.fetch_add(1);
            return;
        }
        int k = p->find(from);
        if (k < 0 && from.ip != 0) {           // where it was seen: an address of its own
            p->cand[DirectPath::kSeen] = from;
            p->replied[DirectPath::kSeen] = TimePoint{};
            k = DirectPath::kSeen;
        }
        if (d[3] == kTypeProbe) {
            std::memcpy(reply, d, kProbeSize);
            putHeader(reply, kTypeProbeReply, m.opt.peer_id, p->key, sender);
            answer = true;
            // It reaches us: probe it back soon, if the path isn't up.
            if (p->state != DirectPath::Up && now - p->last_probe > std::chrono::milliseconds(50))
                p->last_probe = TimePoint{};
        } else if (d[3] == kTypeProbeReply) {
            const std::uint64_t stamp = getLE64(d + 20);
            const std::uint64_t t = monotonicMicros() | (1ULL << 63);
            if (t < stamp || t - stamp > static_cast<std::uint64_t>(kAckMaxRoundTripUs)) {
                dropped_.fetch_add(1);
                return;
            }
            p->rtt_ms = static_cast<double>(t - stamp) / 1000.0;
            if (k >= 0) p->replied[k] = now;
            p->last_reply = now;
            p->state = DirectPath::Up;
            p->choose(now);
            if (p->active < 0) p->state = DirectPath::Opening;   // can't happen: k answered
            return;
        }
    }
    if (answer) {
        if (udp::sendTo(m.fd, reply, sizeof(reply), from, 0, false) == 0) sent_.fetch_add(1);
        return;
    }
    if (m.lose()) {                            // data: the test's loss, as through the relay
        dropped_.fetch_add(1);
        return;
    }
    received_.fetch_add(1);
    direct_received_.fetch_add(1);
    if (on_data) on_data(sender, d + kClientHeader, n - kClientHeader, true);
}

void Transport::loop() {
    Impl& m = *impl_;
    const thread::NetworkScheduling sched = thread::makeNetworkThread();
    m.realtime.store(sched == thread::NetworkScheduling::Realtime);
    m.latency_critical.store(sched == thread::NetworkScheduling::LatencyCritical);
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
        const int ready = compat::poll(p, n_fds, m.wait_ms);
        if (ready == 0) {                      // the timed wait ran out: how long it took
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
            m.timeouts.fetch_add(1, std::memory_order_relaxed);
            if (us > 1000L * m.wait_ms + 5000) m.over_5ms.fetch_add(1, std::memory_order_relaxed);
            if (us > m.longest_us.load(std::memory_order_relaxed)) m.longest_us.store(us, std::memory_order_relaxed);
        }
        if (n_fds == 2 && (p[1].revents & POLLIN)) {
            // How long after the wake() it runs: the wake-up delay.
            const std::int64_t at = m.woken_at_us.load();
            const std::int64_t us = static_cast<std::int64_t>(monotonicMicros()) - at;
            if (at > 0 && us >= 0) {
                if (us > 5000) m.wake_over_5ms.fetch_add(1, std::memory_order_relaxed);
                if (us > m.wake_longest_us.load(std::memory_order_relaxed))
                    m.wake_longest_us.store(us, std::memory_order_relaxed);
            }
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
            if (from.ip == m.relay.ip && from.port == m.relay.port)
                handleDatagram(m.buf, n);
            else
                handleDirect(m.buf, n, from.ip, from.port);
        }
        if (Clock::now() - m.last_hello >= std::chrono::milliseconds(kHelloIntervalMs)) sendHello();
        probePaths();
        if (on_tick) on_tick();
    }
    if (on_closing) on_closing();
}

}  // namespace live
}  // namespace o11proto
