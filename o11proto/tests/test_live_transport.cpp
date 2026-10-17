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
    void wire(live::Transport& t) {
        t.on_data = [this](std::uint32_t from, const std::uint8_t* p, std::size_t n) {
            std::lock_guard<std::mutex> lock(m);
            got.emplace_back(from, Bytes(p, p + n));
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

// A Live player: the TCP client and its transport.
struct Player {
    Client c;
    live::Transport t;
    Inbox in;
    std::uint32_t id() const { return c.liveMessage().peer_id; }
    bool listsBound(std::uint32_t peer) const {
        for (const auto& p : c.liveMessage().peers)
            if (p.id == peer) return (p.flags & live::kPeerBound) != 0;
        return false;
    }
    bool login(Room& r, const std::string& name) {
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
    live::Transport t;
    Inbox in;
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
    // A datagram from another port of the server's address: refused by connect().
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
    live::Transport t;
    std::atomic<int> ticks{0};
    std::atomic<bool> closed{false};
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

// ----------------------------------------------------- through the server

TEST(two_players_through_the_server) {
    Room r;
    Player a, b;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    REQUIRE(r.until([&] { return a.c.liveMessage().peers.size() == 1 && b.c.liveMessage().peers.size() == 1; }));
    REQUIRE(a.startTransport());
    REQUIRE(b.startTransport());
    // Both bind within a round trip or two; the others see it within 1 s.
    REQUIRE(r.until([&] { return a.listsBound(b.id()) && b.listsBound(a.id()); }, 1500));
    CHECK(a.t.serverAnswered() && b.t.serverAnswered());
    std::printf("    server round trip %.3f ms\n", a.t.serverRoundTripMs());
    CHECK(a.t.serverRoundTripMs() >= 0 && a.t.serverRoundTripMs() < 50);

    std::atomic<int> phase{0};
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
    Player a, b;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    REQUIRE(r.until([&] { return a.c.liveMessage().peers.size() == 1; }));
    REQUIRE(a.startTransport());
    REQUIRE(b.startTransport());
    REQUIRE(r.until([&] { return a.listsBound(b.id()) && b.listsBound(a.id()); }, 1500));
    std::atomic<bool> sending{true};
    a.t.on_tick = [&] {
        static thread_local TimePoint last;
        if (sending.load() && Clock::now() - last >= std::chrono::milliseconds(5)) {
            last = Clock::now();
            std::uint8_t p[8] = {0x81};
            a.t.send(0, p, sizeof(p));
        }
    };
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
    Player a, b;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    REQUIRE(r.until([&] { return a.c.liveMessage().peers.size() == 1; }));
    REQUIRE(b.startTransport());
    std::uint32_t bid = b.id();
    TimePoint last = Clock::now();
    a.t.on_tick = [&, bid] {                   // a stream's 200 datagrams/s, within the rate limit
        if (Clock::now() - last < std::chrono::milliseconds(5)) return;
        last = Clock::now();
        std::uint8_t p[100] = {0x81};
        a.t.send(bid, p, sizeof(p));
    };
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
}

TEST(load_eight_players) {                     // each streams to the room for 2 s
    Room r;
    const int kPlayers = 8;
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
    for (auto& p : ps) REQUIRE(p->startTransport());
    REQUIRE(r.until([&] {
        for (auto& p : ps)
            for (auto& q : ps)
                if (p != q && !p->listsBound(q->id())) return false;
        return true;
    }, 3000));
    // 200 datagrams/s (frame 240) of 105 bytes, as a 128 kbps stream averages.
    std::atomic<bool> go{true};
    std::vector<std::unique_ptr<std::atomic<int>>> sent;
    for (int i = 0; i < kPlayers; ++i) sent.emplace_back(new std::atomic<int>(0));
    TimePoint t0 = Clock::now();
    for (int i = 0; i < kPlayers; ++i) {
        Player* p = ps[i].get();
        std::atomic<int>* n = sent[i].get();
        p->t.on_tick = [p, n, &go, t0] {
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
