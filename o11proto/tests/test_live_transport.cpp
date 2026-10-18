// Live mode, the client side of UDP (PROTOCOL.md 15.5-15.6): the parsing
// rules of the Live message, the hello schedule and the client's drop
// rules against a fake relay (a raw test socket), then live::Transport
// through a real server: payloads both ways, Live off, restarts and eight
// players at once. The server runs on its own thread; the main thread pumps
// the TCP clients.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "o11proto/client.h"
#include "o11proto/live.h"
#include "o11proto/live_transport.h"
#include "o11proto/realtime.h"
#include "o11proto/server.h"
#include "socket_compat.h"
#include "udp_compat.h"

using namespace o11proto;
using Bytes = std::vector<std::uint8_t>;

namespace {

// ----------------------------------------------------------- a fake relay

// A raw socket on 127.0.0.1 that plays the relay for one transport.
struct FakeRelay {
    socket_t fd = kInvalidSocket;
    std::uint16_t port = 0;
    udp::Addr peer;                            // the transport's address, from its first datagram
    FakeRelay() {
        fd = udp::open(1 << 20, false);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        socklen_t len = sizeof(sa);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
        port = ntohs(sa.sin_port);
    }
    ~FakeRelay() { compat::closeSocket(fd); }
    bool recv(Bytes& out, int timeout_ms = 500) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        std::uint8_t buf[2048];
        for (;;) {
            std::size_t n = 0;
            bool trunc = false, has_local = false;
            udp::Addr from, local;
            if (udp::receive(fd, buf, sizeof(buf), n, trunc, from, local, has_local) == udp::Recv::Datagram) {
                peer = from;
                out.assign(buf, buf + n);
                return true;
            }
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    void send(const Bytes& b) { udp::sendTo(fd, b.data(), b.size(), peer, 0, false); }
};

Bytes serverHeader(std::uint8_t type, std::uint32_t origin) {
    Bytes b(live::kServerHeader);
    b[0] = 'N';
    b[1] = 'L';
    b[2] = 1;
    b[3] = type;
    live::putLE32(&b[4], origin);
    return b;
}
Bytes ack(std::uint64_t stamp, std::uint64_t cookie) {
    Bytes b = serverHeader(live::kTypeHello, 0);
    b.resize(live::kHelloAckSize);
    live::putLE64(&b[8], stamp);
    live::putLE64(&b[16], cookie);
    return b;
}
Bytes dataFrom(std::uint32_t origin, std::size_t payload, std::uint8_t fill = 0x81) {
    Bytes b = serverHeader(live::kTypeData, origin);
    b.resize(live::kServerHeader + payload, fill);
    return b;
}

// What a transport delivered.
struct Inbox {
    std::mutex m;
    std::vector<std::pair<std::uint32_t, Bytes>> got;
    int directs = 0;                           // of them, from the sender directly
    void wire(live::Transport& t) {
        t.on_data = [this](std::uint32_t from, const std::uint8_t* p, std::size_t n, bool direct) {
            std::lock_guard<std::mutex> lock(m);
            got.emplace_back(from, Bytes(p, p + n));
            if (direct) ++directs;
        };
    }
    std::size_t size() {
        std::lock_guard<std::mutex> lock(m);
        return got.size();
    }
    std::pair<std::uint32_t, Bytes> at(std::size_t i) {
        std::lock_guard<std::mutex> lock(m);
        return got.at(i);
    }
};

bool waitFor(const std::function<bool()>& pred, int timeout_ms = 2000) {
    TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

live::Transport::Options fakeOptions(const FakeRelay& r) {
    live::Transport::Options o;
    o.server_address = "127.0.0.1";
    o.port = r.port;
    o.peer_id = 7;
    o.key = 0x1122334455667788ULL;
    return o;
}

// -------------------------------------------- a server on its own thread

struct Room {
    Server server;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::vector<Client*> clients;

    explicit Room(LiveMode mode = LiveMode::On) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.anonymous_privs = kPrivChat | kPrivVote | kPrivMulti;
        cfg.live.mode = mode;
        CHECK(server.start(cfg));
        thread = std::thread([this] {
            while (!stop.load()) server.poll(1);
        });
    }
    ~Room() {                                  // the players (declared later) are gone by now
        stop.store(true);
        thread.join();
    }
    std::uint16_t port() const { return server.port(); }
    void forget(Client* c) { clients.erase(std::remove(clients.begin(), clients.end(), c), clients.end()); }
    void pump() {
        for (Client* c : clients) c->poll(0);
    }
    bool until(const std::function<bool()>& pred, int timeout_ms = 2000) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (Clock::now() >= deadline) return false;
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

// A Live player: the TCP client and its transport. Its transport stops
// first when it goes, before the inbox its on_data writes (and the test's
// state its on_tick uses, declared before the player).
struct Player {
    Client c;
    live::Transport t;
    Inbox in;
    Room* room = nullptr;
    ~Player() {
        t.stop();
        if (room) room->forget(&c);
    }
    std::uint32_t id() const { return c.liveMessage().peer_id; }
    bool listsBound(std::uint32_t peer) const {
        for (const auto& p : c.liveMessage().peers)
            if (p.id == peer) return (p.flags & live::kPeerBound) != 0;
        return false;
    }
    bool login(Room& r, const std::string& name) {
        room = &r;
        r.clients.push_back(&c);
        Client::Options o;
        o.username = "anonymous:" + name;
        o.codec_caps = kCapOpus;
        o.live = true;
        ChannelInfoRecord ch;
        ch.name = "ch0";
        c.setChannels({ch});
        if (!c.connect("127.0.0.1", r.port(), o)) return false;
        return r.until([&] { return c.loggedIn() && c.hasLiveMessage(); });
    }
    bool startTransport() {
        const live::ServerLive& m = c.liveMessage();
        live::Transport::Options o;
        o.server_address = c.serverAddress();
        o.port = m.udp_port ? m.udp_port : c.serverPort();
        o.peer_id = m.peer_id;
        o.key = m.key;
        in.wire(t);
        std::string err;
        bool ok = t.start(o, err);
        if (!ok) std::printf("    transport: %s\n", err.c_str());
        return ok;
    }
};

// A host's audio thread: it wakes the transports every half millisecond, as
// the engine's audio does when it has queued, from before their restarts to
// after. It is scheduled as an audio thread of that period
// (o11proto/realtime.h), as a host's is: on macOS an ordinary thread's sleep
// of 0.5 ms can take 9, and then the test would measure its own waits. It
// stops when it goes, so it is declared after what it wakes.
struct Waker {
    std::atomic<bool> stop{false};
    std::atomic<bool> realtime{false};         // makeAudioThread() took
    std::atomic<int> sent{0};                  // rounds of wakes
    std::thread thread;
    explicit Waker(std::vector<live::Transport*> ts)
        : thread([this, ts] {
              realtime.store(o11proto::thread::makeAudioThread(0.0005));
              while (!stop.load()) {
                  for (live::Transport* t : ts) t->wake();
                  sent.fetch_add(1);
                  std::this_thread::sleep_for(std::chrono::microseconds(500));
              }
          }) {}
    ~Waker() {
        stop.store(true);
        thread.join();
    }
};

}  // namespace

// ------------------------------------------------------- the Live message

TEST(live_message_parse_rules) {
    live::ServerLive m;
    m.flags = live::kFlagRoomOn | live::kFlagUdpOpen | 0x80;   // an unknown bit is kept, not refused
    m.udp_port = 4000;
    m.live_kbps = 96;
    m.peer_id = 5;
    m.key = 0x0102030405060708ULL;
    m.frame_samples = 120;
    m.max_peers = 3;
    m.peers.push_back({9, live::kPeerBound, "bob"});
    m.peers.push_back({0, 0, "zero"});
    m.peers.push_back({5, 0, "me"});
    m.peers.push_back({11, 0, "carol"});
    Bytes b = live::encode(m);
    live::ServerLive d;
    REQUIRE(live::decode(b, d));
    CHECK(d.roomOn() && d.udpOpen() && !d.maySend());
    CHECK_EQ(d.udp_port, 4000);
    CHECK_EQ(d.live_kbps, 96);
    CHECK_EQ(d.key, 0x0102030405060708ULL);
    CHECK_EQ(d.frame_samples, 120);
    CHECK_EQ(d.max_peers, 3);
    REQUIRE_EQ(d.peers.size(), 2u);            // id 0 and the own id are skipped
    CHECK(d.peers[0].id == 9 && d.peers[0].flags == live::kPeerBound && d.peers[0].name == "bob");
    CHECK(d.peers[1].id == 11 && d.peers[1].name == "carol");

    Bytes s(b.begin(), b.begin() + 31);
    CHECK(!live::decode(s, d));                // shorter than 32
    Bytes v0 = b;
    v0[0] = 0;
    CHECK(!live::decode(v0, d));               // version 0
    Bytes f31 = b;
    f31[1] = 31;
    CHECK(!live::decode(f31, d));              // fixed length below 32
    Bytes big = b;
    big[1] = static_cast<std::uint8_t>(std::min<std::size_t>(255, b.size() + 1));
    CHECK(!live::decode(big, d));              // fixed length past the payload

    // A later version with a longer fixed part: read as version 1, the extra bytes skipped.
    Bytes v2(b.begin(), b.begin() + 32);
    v2[0] = 2;
    v2[1] = 40;
    v2.insert(v2.end(), 8, 0xEE);
    v2.insert(v2.end(), b.begin() + 32, b.end());
    REQUIRE(live::decode(v2, d));
    CHECK_EQ(d.version, 2);
    CHECK_EQ(d.peers.size(), 2u);
    CHECK_EQ(d.key, 0x0102030405060708ULL);

    // A record without its NUL ends the list; so does one with fewer than 6 bytes.
    Bytes cut = live::encode(m);
    cut.pop_back();
    REQUIRE(live::decode(cut, d));
    CHECK_EQ(d.peers.size(), 1u);
    Bytes stub(b.begin(), b.begin() + 32);
    stub.insert(stub.end(), {9, 0, 0, 0, 0});  // 5 bytes
    REQUIRE(live::decode(stub, d));
    CHECK(d.peers.empty());

    // At most 256 records are read, skipped ones included.
    live::ServerLive many = m;
    many.peers.clear();
    for (std::uint32_t i = 1; i <= 300; ++i) many.peers.push_back({100 + i, 0, "p"});
    REQUIRE(live::decode(live::encode(many), d));
    CHECK_EQ(d.peers.size(), 256u);
}

TEST(client_needs_live_and_login) {            // 0x07 is ignored without bit 16
    Room r(LiveMode::Yes);
    Client plain;
    bool got = false;
    plain.on_live = [&](const live::ServerLive&) { got = true; };
    r.clients.push_back(&plain);
    Client::Options o;
    o.username = "anonymous:plain";
    REQUIRE(plain.connect("127.0.0.1", r.port(), o));
    REQUIRE(r.until([&] { return plain.loggedIn(); }));
    r.until([] { return false; }, 300);
    CHECK(!got && !plain.hasLiveMessage());
    CHECK_EQ(plain.serverAddress(), "127.0.0.1");
    CHECK_EQ(plain.serverPort(), r.port());
}

// ----------------------------------------------------- against a fake relay

TEST(start_failures) {
    live::Transport t;
    std::string err;
    live::Transport::Options o;
    o.server_address = "127.0.0.1";
    o.port = 2049;
    CHECK(!t.start(o, err));                   // no peer id
    CHECK_EQ(err, "no peer ID or server address");
    o.peer_id = 1;
    o.server_address = "";
    CHECK(!t.start(o, err));
    CHECK_EQ(err, "no peer ID or server address");
    o.server_address = "::1";
    CHECK(!t.start(o, err));                   // IPv4 only
    CHECK(!t.running());
}

TEST(hello_schedule) {                         // 29, against a fake relay
    FakeRelay r;
    live::Transport t;
    std::string err;
    TimePoint t0 = Clock::now();
    REQUIRE(t.start(fakeOptions(r), err));
    Bytes h;
    REQUIRE(r.recv(h, 100));                   // the first hello at once
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(100));
    REQUIRE_EQ(h.size(), live::kHelloSize);
    CHECK(h[0] == 'N' && h[1] == 'L' && h[2] == 1 && h[3] == live::kTypeHello);
    CHECK_EQ(live::getLE32(&h[4]), 7u);
    CHECK_EQ(live::getLE64(&h[8]), 0x1122334455667788ULL);
    CHECK_EQ(live::getLE32(&h[16]), 0u);
    const std::uint64_t stamp = live::getLE64(&h[20]);
    CHECK((stamp >> 63) == 1);
    CHECK_EQ(live::getLE64(&h[28]), 0u);
    CHECK_EQ(live::getLE32(&h[36]), 0u);
    CHECK(!t.serverAnswered());

    r.send(ack(stamp, 0xC0C0C0C0ULL));
    TimePoint sent = Clock::now();
    REQUIRE(r.recv(h, 100));                   // a new cookie: the next hello at once
    CHECK(Clock::now() - sent < std::chrono::milliseconds(50));
    CHECK_EQ(live::getLE64(&h[28]), 0xC0C0C0C0ULL);
    CHECK(t.serverAnswered());
    CHECK(t.serverRoundTripMs() >= 0 && t.serverRoundTripMs() < 100);
    CHECK_EQ(t.cookie(), 0xC0C0C0C0ULL);
    r.send(ack(live::getLE64(&h[20]), 0xC0C0C0C0ULL));   // the same cookie: nothing more at once
    TimePoint second = Clock::now();
    REQUIRE(r.recv(h, 2500));
    double gap = std::chrono::duration<double>(Clock::now() - second).count();
    std::printf("    next hello after %.3f s\n", gap);
    CHECK(gap > 1.8 && gap < 2.2);
    CHECK_EQ(live::getLE64(&h[28]), 0xC0C0C0C0ULL);
    t.stop();
    CHECK(!t.running());
}

TEST(client_drop_rules) {                      // 4.2's list, and replies that aren't ours
    FakeRelay r;
    Inbox in;                                  // before t: its on_data writes it until t stops
    live::Transport t;
    in.wire(t);
    std::string err;
    REQUIRE(t.start(fakeOptions(r), err));
    Bytes h;
    REQUIRE(r.recv(h));
    const std::uint64_t stamp = live::getLE64(&h[20]);
    r.send(ack(stamp, 0xAAAA));
    REQUIRE(r.recv(h));                        // the hello with the cookie
    REQUIRE_EQ(t.cookie(), 0xAAAAu);

    // Replies that must not change the cookie.
    r.send(ack(0x8000000000000005ULL, 0xBAD1));               // a timestamp never sent
    r.send(ack(0, 0xBAD2));                                   // timestamp 0
    Bytes long_ack = ack(stamp, 0xBAD3);
    long_ack.push_back(0);
    r.send(long_ack);                                         // 25 bytes
    Bytes origin_ack = ack(stamp, 0xBAD4);
    live::putLE32(&origin_ack[4], 3);
    r.send(origin_ack);                                       // origin not 0
    Bytes v2_ack = ack(stamp, 0xBAD5);
    v2_ack[2] = 2;
    r.send(v2_ack);                                           // version 2
    // Data that must be dropped.
    r.send(dataFrom(0, 10));                                  // origin 0
    r.send(dataFrom(9, 0));                                   // no payload
    r.send(dataFrom(9, 1453));                                // payload too long
    r.send(dataFrom(9, 1500 - 8 + 1));                        // over 1500 bytes
    Bytes magic = dataFrom(9, 10);
    magic[0] = 'X';
    r.send(magic);
    Bytes type3 = dataFrom(9, 10);
    type3[3] = 3;
    r.send(type3);
    // Data that passes.
    r.send(dataFrom(9, 1));
    r.send(dataFrom(9, 1452, 0x42));
    // A datagram from another port of the server's address: not the relay's,
    // nor a peer's (no path, no pair key).
    FakeRelay other;
    other.peer = r.peer;
    other.send(dataFrom(9, 20));
    REQUIRE(waitFor([&] { return in.size() >= 2; }, 500));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(in.size(), 2u);
    CHECK(in.at(0).first == 9 && in.at(0).second.size() == 1);
    CHECK(in.at(1).second.size() == 1452 && in.at(1).second[1451] == 0x42);
    CHECK_EQ(t.cookie(), 0xAAAAu);
    CHECK(t.datagramsDropped() >= 11);
    t.stop();
}

TEST(sends_and_closing) {                      // send() from the network thread; on_closing last
    FakeRelay r;
    std::atomic<int> ticks{0};                 // before t: its callbacks use them until it stops
    std::atomic<bool> closed{false};
    live::Transport t;
    t.on_tick = [&] {
        if (ticks.fetch_add(1) == 5) {
            std::uint8_t p[3] = {1, 2, 3};
            t.send(42, p, sizeof(p));
            t.send(42, p, 0);                  // empty: refused
        }
    };
    t.on_closing = [&] {
        std::uint8_t p[1] = {0xFF};
        t.send(0, p, 1);
        closed.store(true);
    };
    std::string err;
    REQUIRE(t.start(fakeOptions(r), err));
    Bytes d;
    REQUIRE(r.recv(d));                        // the hello
    REQUIRE(r.recv(d));
    REQUIRE_EQ(d.size(), live::kClientHeader + 3);
    CHECK_EQ(d[3], live::kTypeData);
    CHECK_EQ(live::getLE32(&d[4]), 7u);
    CHECK_EQ(live::getLE32(&d[16]), 42u);
    CHECK(d[20] == 1 && d[22] == 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ticks.load() >= 10);                 // at least every millisecond, roughly
    t.stop();
    CHECK(closed.load());
    REQUIRE(r.recv(d));
    CHECK(d.size() == live::kClientHeader + 1 && d[20] == 0xFF && live::getLE32(&d[16]) == 0);
}

// A longer housekeeping wait: alone, a pass every `wait_ms`; with a host
// that wakes it, a pass for each wake, and the wake-up delay measured.
TEST(housekeeping_wait_and_wakes) {
    FakeRelay r;
    std::atomic<int> ticks{0};                 // before t: its callbacks use it until it stops
    live::Transport t;
    t.on_tick = [&] { ticks.fetch_add(1); };
    live::Transport::Options o = fakeOptions(r);
    o.wait_ms = 20;
    std::string err;
    REQUIRE(t.start(o, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int t0 = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int alone = ticks.load() - t0;
    int woken = 0, sent = 0;
    bool realtime = false;
    {
        Waker host({&t});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int t1 = ticks.load(), s1 = host.sent.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        woken = ticks.load() - t1;
        sent = host.sent.load() - s1;
        realtime = host.realtime.load();
    }
    const live::Transport::Timing tm = t.timing();
    t.stop();
    std::printf("    in 200 ms: %d passes alone, %d woken every 0.5 ms (the host woke it %d times, %s); %llu wakes, "
                "the longest wake-up delay %.2f ms (%llu over 5 ms); waits of %d ms, the longest %.1f ms\n",
                alone, woken, sent, realtime ? "an audio thread" : "an ordinary thread",
                static_cast<unsigned long long>(tm.wakes), tm.wake_longest_ms,
                static_cast<unsigned long long>(tm.wake_over_5ms), tm.wait_ms, tm.longest_ms);
#ifdef _WIN32
    CHECK_EQ(tm.wait_ms, 1);                   // wake() does nothing here: every millisecond
#else
    CHECK_EQ(tm.wait_ms, 20);
    CHECK(alone >= 3 && alone <= 20);          // about 10, the hello's reply aside
    CHECK(woken >= 4 * alone);
    CHECK(tm.wakes >= 20);
    CHECK(tm.wake_longest_ms >= 0 && tm.wake_longest_ms < 1000);
#endif
}

TEST(stop_ends_the_callbacks) {                // what an owner relies on, through restarts while data pours in
    FakeRelay r;
    std::atomic<bool> pour{true}, stopped{false};   // before t: its callbacks use them until it stops
    std::atomic<int> late{0}, data{0}, closings{0};
    live::Transport t;
    t.on_data = [&](std::uint32_t, const std::uint8_t*, std::size_t, bool) {
        late.fetch_add(stopped.load() ? 1 : 0);
        data.fetch_add(1);
    };
    t.on_tick = [&] { late.fetch_add(stopped.load() ? 1 : 0); };
    t.on_closing = [&] {
        late.fetch_add(stopped.load() ? 1 : 0);
        closings.fetch_add(1);
    };
    // The relay: data every 50 us to where the latest hello came from (each start has a new socket).
    std::thread relay([&] {
        const Bytes d = dataFrom(9, 100);
        Bytes in;
        while (pour.load()) {
            r.recv(in, 0);
            if (r.peer.port != 0) r.send(d);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    {
        Waker host({&t});
        std::string err;
        for (int i = 0; i < 20; ++i) {
            stopped.store(false);
            const bool started = t.start(fakeOptions(r), err);
            CHECK(started);
            if (!started) break;               // (not REQUIRE: the relay's thread is joined below)
            const int before = data.load();
            CHECK(waitFor([&] { return data.load() >= before + 20; }, 1000));
            t.stop();                          // the callbacks end here,
            stopped.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));   // so none comes in this time
        }
    }
    pour.store(false);
    relay.join();
    CHECK_EQ(late.load(), 0);
    CHECK_EQ(closings.load(), 20);
    std::printf("    %d datagrams delivered over 20 starts\n", data.load());
}

TEST(send_gap_and_hold) {                      // the test hooks; a restart drops what is held
    FakeRelay r;
    std::atomic<int> want{0};                  // before t: its callbacks use them until it stops
    std::uint32_t seq = 0;                     // the network thread's
    live::Transport t;
    t.on_tick = [&] {                          // numbered datagrams, as many as asked for
        for (; want.load() > 0; want.fetch_sub(1)) {
            std::uint8_t p[4];
            live::putLE32(p, seq++);
            t.send(0, p, sizeof(p));
        }
    };
    auto sendNow = [&](int n) {                // and wait until the network thread has sent them
        want.fetch_add(n);
        return waitFor([&] { return want.load() == 0; }, 500);
    };
    auto arrived = [&] {                       // the numbers the relay has, in order, the hellos aside
        std::vector<std::uint32_t> got;
        Bytes d;
        while (r.recv(d, 50))
            if (d.size() == live::kClientHeader + 4 && d[3] == live::kTypeData)
                got.push_back(live::getLE32(&d[live::kClientHeader]));
        return got;
    };
    using Seqs = std::vector<std::uint32_t>;
    std::string err;
    REQUIRE(t.start(fakeOptions(r), err));
    REQUIRE(sendNow(2));
    CHECK(arrived() == Seqs({0, 1}));
    t.setTestSendHold(300);
    REQUIRE(sendNow(3));
    CHECK(arrived().empty());                  // held
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(sendNow(1));                       // the first after the hold: then all at once, in order
    CHECK(arrived() == Seqs({2, 3, 4, 5}));
    t.setTestSendGap(300);
    REQUIRE(sendNow(3));                       // dropped
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(sendNow(1));
    CHECK(arrived() == Seqs({9}));
    t.setTestSendHold(300);
    REQUIRE(sendNow(2));                       // held, and dropped by the restart
    t.stop();
    REQUIRE(t.start(fakeOptions(r), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(sendNow(1));
    CHECK(arrived() == Seqs({12}));
    t.stop();
}

// ----------------------------------------------------- through the server

TEST(two_players_through_the_server) {
    Room r;
    std::atomic<int> phase{0};                 // before the players: their callbacks read it until they stop
    Player a, b;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    REQUIRE(r.until([&] { return a.c.liveMessage().peers.size() == 1 && b.c.liveMessage().peers.size() == 1; }));
    a.t.on_tick = [&] {
        if (phase.load() == 1) {
            std::uint8_t p[5] = {0x81, 1, 2, 3, 4};
            a.t.send(0, p, sizeof(p));         // to the room
            phase.store(2);
        }
    };
    const std::uint32_t aid = a.id();
    b.t.on_tick = [&, aid] {
        if (phase.load() == 3) {
            std::uint8_t p[1452];
            for (std::size_t i = 0; i < sizeof(p); ++i) p[i] = static_cast<std::uint8_t>(i);
            b.t.send(aid, p, sizeof(p));       // to one player
            phase.store(4);
        }
    };
    REQUIRE(a.startTransport());
    REQUIRE(b.startTransport());
    // Both bind within a round trip or two; the others see it within 1 s.
    REQUIRE(r.until([&] { return a.listsBound(b.id()) && b.listsBound(a.id()); }, 1500));
    CHECK(a.t.serverAnswered() && b.t.serverAnswered());
    std::printf("    server round trip %.3f ms\n", a.t.serverRoundTripMs());
    CHECK(a.t.serverRoundTripMs() >= 0 && a.t.serverRoundTripMs() < 50);
    phase.store(1);
    REQUIRE(r.until([&] { return b.in.size() == 1; }));
    CHECK(b.in.at(0).first == a.id() && b.in.at(0).second == Bytes({0x81, 1, 2, 3, 4}));
    phase.store(3);
    REQUIRE(r.until([&] { return a.in.size() == 1; }));
    CHECK(a.in.at(0).first == b.id() && a.in.at(0).second.size() == 1452 && a.in.at(0).second[300] == 44);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(b.in.size(), 1u);                 // no copy back to the sender
}

TEST(live_off_stops_forwarding) {              // 21, through transports
    Room r;
    std::atomic<bool> sending{false};          // before the players: a's callbacks read it until a stops
    Player a, b;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    REQUIRE(r.until([&] { return a.c.liveMessage().peers.size() == 1; }));
    a.t.on_tick = [&] {
        static thread_local TimePoint last;
        if (sending.load() && Clock::now() - last >= std::chrono::milliseconds(5)) {
            last = Clock::now();
            std::uint8_t p[8] = {0x81};
            a.t.send(0, p, sizeof(p));
        }
    };
    REQUIRE(a.startTransport());
    REQUIRE(b.startTransport());
    REQUIRE(r.until([&] { return a.listsBound(b.id()) && b.listsBound(a.id()); }, 1500));
    sending.store(true);
    REQUIRE(r.until([&] { return b.in.size() >= 10; }));
    a.c.say("!live off");
    REQUIRE(r.until([&] { return !a.c.liveMessage().roomOn() && !b.c.liveMessage().roomOn(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::size_t before = b.in.size();
    r.until([] { return false; }, 300);
    CHECK_EQ(b.in.size(), before);             // nothing forwarded while off
    CHECK(a.t.datagramsSent() > 0);
    a.c.say("!live on");
    REQUIRE(r.until([&] { return b.c.liveMessage().roomOn(); }));
    CHECK(r.until([&] { return b.in.size() > before + 5; }));
    sending.store(false);
}

TEST(restarts_survive) {                       // 45: stop and start while sending, 50 times
    Room r;
    TimePoint last = Clock::now();             // before the players: a's callbacks use it until a stops
    Player a, b;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    REQUIRE(r.until([&] { return a.c.liveMessage().peers.size() == 1; }));
    REQUIRE(b.startTransport());
    std::uint32_t bid = b.id();
    a.t.on_tick = [&, bid] {                   // a stream's 200 datagrams/s, within the rate limit
        if (Clock::now() - last < std::chrono::milliseconds(5)) return;
        last = Clock::now();
        std::uint8_t p[100] = {0x81};
        a.t.send(bid, p, sizeof(p));
    };
    Waker host({&a.t, &b.t});
    int answered = 0;
    for (int i = 0; i < 50; ++i) {
        REQUIRE(a.startTransport());
        r.until([&] { return a.t.serverAnswered(); }, 200);
        answered += a.t.serverAnswered() ? 1 : 0;
        a.t.stop();
    }
    std::printf("    %d of 50 restarts answered\n", answered);
    CHECK(answered >= 45);
    REQUIRE(a.startTransport());
    CHECK(r.until([&] { return a.t.serverAnswered(); }));
    const std::size_t had = b.in.size();
    CHECK(r.until([&] { return b.in.size() > had; }));
}                                              // the host stops, then b goes while a streams to it

TEST(players_go_while_streamed_to) {           // each one's transport stops before what its callbacks use goes
    Room r;
    std::atomic<std::uint32_t> to{0};          // before the players: a's callbacks use them until a stops
    TimePoint last = Clock::now();
    Player a;
    REQUIRE(a.login(r, "a"));
    a.t.on_tick = [&] {                        // 200 datagrams/s to the latest player
        const std::uint32_t dest = to.load();
        if (dest == 0 || Clock::now() - last < std::chrono::milliseconds(5)) return;
        last = Clock::now();
        std::uint8_t p[100] = {0x81};
        a.t.send(dest, p, sizeof(p));
    };
    REQUIRE(a.startTransport());
    int received = 0;
    for (int i = 0; i < 10; ++i) {
        std::unique_ptr<Player> b(new Player);
        REQUIRE(b->login(r, "b" + std::to_string(i)));
        REQUIRE(b->startTransport());
        to.store(b->id());
        Waker host({&a.t, &b->t});
        received += r.until([&] { return b->in.size() >= 5; }) ? 1 : 0;
        // Each goes at another point between two of a's datagrams.
        std::this_thread::sleep_for(std::chrono::microseconds(500 * i));
    }                                          // the host stops, then b goes while a streams to it
    std::printf("    %d of 10 players received a's stream before they went\n", received);
    CHECK(received >= 9);
}

TEST(load_eight_players) {                     // each streams to the room for 2 s
    Room r;
    const int kPlayers = 8;
    // Before the players: their callbacks use these until they stop.
    // 200 datagrams/s (frame 240) of 105 bytes, as a 128 kbps stream averages.
    std::atomic<bool> go{false};
    TimePoint t0;
    std::vector<std::unique_ptr<std::atomic<int>>> sent;
    for (int i = 0; i < kPlayers; ++i) sent.emplace_back(new std::atomic<int>(0));
    std::vector<std::unique_ptr<Player>> ps;
    for (int i = 0; i < kPlayers; ++i) {
        ps.emplace_back(new Player);
        REQUIRE(ps.back()->login(r, "p" + std::to_string(i)));
    }
    REQUIRE(r.until([&] {
        for (auto& p : ps)
            if (p->c.liveMessage().peers.size() != kPlayers - 1) return false;
        return true;
    }));
    for (int i = 0; i < kPlayers; ++i) {
        Player* p = ps[i].get();
        std::atomic<int>* n = sent[i].get();
        p->t.on_tick = [p, n, &go, &t0] {
            if (!go.load()) return;
            double el = std::chrono::duration<double>(Clock::now() - t0).count();
            while (n->load() < static_cast<int>(el * 200)) {
                std::uint8_t d[105];
                std::memset(d, n->load() & 0xFF, sizeof(d));
                d[0] = 0x81;
                p->t.send(0, d, sizeof(d));
                n->fetch_add(1);
            }
        };
    }
    for (auto& p : ps) REQUIRE(p->startTransport());
    REQUIRE(r.until([&] {
        for (auto& p : ps)
            for (auto& q : ps)
                if (p != q && !p->listsBound(q->id())) return false;
        return true;
    }, 3000));
    t0 = Clock::now();
    go.store(true);
    r.until([] { return false; }, 2000);
    go.store(false);
    r.until([] { return false; }, 200);
    int total_sent = 0;
    for (auto& n : sent) total_sent += n->load();
    std::size_t least = SIZE_MAX;
    for (int i = 0; i < kPlayers; ++i) {
        int expect = total_sent - sent[i]->load();
        least = std::min(least, ps[i]->in.size());
        CHECK(static_cast<double>(ps[i]->in.size()) >= 0.99 * expect);
    }
    std::printf("    %d players, %d datagrams sent in all, the least a player received %zu\n", kPlayers, total_sent,
                least);
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    return testing::runAll();
}
